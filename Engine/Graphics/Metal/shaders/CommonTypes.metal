
#define USE_BOUNDING_SPHERES 1

struct GlobalShaderData
{
	float4x4			View;
	float4x4			Projection;
	float4x4			InvProjection;
	float4x4			ViewProjection;
	float4x4			InvViewProjection;

	float4				CameraPositionAndViewWidth;

	float4				CameraDirectionAndViewHeight;

	uint				NumDirectionalLights;
	float				DeltaTime;
	float2 				padding;
};

struct PerObjectData
{
	float4x4 			World;
	float4x4 			InvWorld;
	float4x4 			WorldViewProjection;
};