#version 450

struct Plane
{
	vec3 normal;
	float distance;
};

struct Frustum
{
	Plane Planes[4];
};

struct Result
{
	bool IsHit;

	vec2 UV;
	vec3 pos;

	int IterationCount;
};

struct Ray
{
	vec3 origin;
	vec3 direction;
};

layout (set = 0, binding = 0) uniform UBO 
{
	mat4 model;
	mat4 view;
	mat4 projection;
	vec3 cameraPos;
	vec3 cameraDir;
	float farPlane;
	float nearPlane;
	float time;
} ubo;

layout (binding = 1) uniform sampler2D samplers[3];
//layout (binding = 1) uniform sampler2D samplerCompute;
layout(set = 0, binding = 2) buffer InFrustum
{
	Frustum Frustums[];
} inFrustum;

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

uint GetGridIndex(vec2 posXY, float viewWidth)
{
	return uint(posXY.x / 16) + (100 * uint(posXY.y / 16));
}

vec4 projectToScreenSpace(vec3 point)
{
	return ubo.projection * vec4(point, 1.0);
}

vec3 projectToViewSpace(vec3 point)
{
	return (ubo.view * vec4(point, 1.0)).xyz;
}

float distanceSquared(vec2 A, vec2 B)
{
	A -= B;
	return dot(A, A);
}

bool query(vec2 z, vec2 uv)
{
	float depths = texture(samplers[0], uv / vec2(1600, 900)).a;
	return z.y < depths && z.x > depths;
}

Result RayMarching(Ray r)
{
	Result result;
	vec3 Begin = r.origin;
	vec3 End = r.origin + r.direction * 10000;

	vec3 v0 = projectToViewSpace(Begin);
	vec3 v1 = projectToViewSpace(End);

	vec4 H0 = projectToScreenSpace(v0);
	vec4 H1 = projectToScreenSpace(v1);

	float k0 = 1.0 / H0.w;
	float k1 = 1.0 / H1.w;

	vec3 Q0 = v0 * k0;
	vec3 Q1 = v1 * k1;

	vec2 P0 = H0.xy * k0;
	vec2 P1 = H1.xy * k1;

	vec2 Size = vec2(1600, 900);
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
		if(result.UV.x > 1600 || result.UV.x < 0 || result.UV.y > 900 || result.UV.y < 0)
		{
			break;
		}
		result.IsHit = query(Depths, result.UV);
		if(result.IsHit) { break; }
	}

	return result;
}

void main() 
{
	vec2 flip_y_uv = vec2(inUV.x, 1.0 - inUV.y);
	vec3 fragPos = texture(samplers[0], flip_y_uv).rgb;
	vec3 normal = normalize(texture(samplers[1], flip_y_uv).rgb * 2.0 - 1.0);
	vec4 albedo = texture(samplers[2], flip_y_uv);

	vec3 viewDir = normalize(ubo.cameraPos - fragPos);
	vec3 reflectDir = normalize(reflect(-viewDir, normal));
	Ray ray;
	ray.origin = fragPos;
	ray.direction = reflectDir;
	Result result = RayMarching(ray);
	
	if(result.IsHit)
	{
		outFragColor = vec4(texture(samplers[2], result.UV / vec2(1600, 900)).xyz, 1);
	}
	else
	{
		outFragColor = albedo;
	}
	//outFragColor = albedo;
}