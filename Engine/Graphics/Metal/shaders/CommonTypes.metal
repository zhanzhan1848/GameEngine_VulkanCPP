
#define USE_BOUNDING_SPHERES 1
#define SHADOWING_MAPPING 1

struct GlobalShaderData
{
	float4x4			View;
	float4x4			Projection;
	float4x4			InvProjection;
	float4x4			ViewProjection;
	float4x4			PreviousViewProjection;
	float4x4			InvViewProjection;

	float4				CameraPositionAndViewWidth;

	float4				CameraDirectionAndViewHeight;

	uint				NumDirectionalLights;
	float				DeltaTime;
	float				FrameCount;
	float 				padding;
};

struct PerObjectData
{
	float4x4 			World;
	float4x4 			InvWorld;
	float4x4 			WorldViewProjection;
};

struct Plane
{
	float3 Normal;
	float  Distance;
};

struct Sphere
{
	float3 Center;
	float  Radius;
};

struct Cone
{
	float3	Tip;
	float	Height;
	float3	Direction;
	float	Radius;
};

#if USE_BOUNDING_SPHERES
// Frustum cone in view space
struct Frustum
{
    float3	ConeDirection;
    float	UnitRadius;
};
#else
// View frustum planes ( in view space)
// Plane order: left, right, top, bottom
// Front and back planes are computed in light culling compute shader
struct Frustum
{
	Plane	Planes[4];
};
#endif

struct LightCullingDispatchParameters
{
	// Number of groups dispatched. (This parameter is not available as an HLSL system value!)
    uint2   NumThreadGroups;

    // Total number of threads dispatched. (Also not available as an HLSL system value!)
    // NOTE: This value may be less than the actual number of threads executed 
    //       if the screen size is not evenly divisible by the block size.
    uint2   NumThreads;

	// Number of lights for culling (doesn't include directional lights, because those can't be culled.)
	uint	NumLights;

	// The index of current depth buffer in SRV descriptor heap
	uint	DepthBufferSrvIndex;
};

// Contains light cullign data that's formatted and ready to be copied
// to a D3D constant/structured buffer as contiguous chunk
struct LightCullingLightInfo
{
	float3		Position;
	float		Range;

	float3		Direction;
#if USE_BOUNDING_SPHERES
	// IF this is set to -1 then the light is a point light
	float		CosPenumbra;
#else
	float		ConeRadius;

	uint		Type;
	float3		_pad;
#endif
};

// Contains light data that's formatted and ready to be copy
// to a D3D constant/structured buffer as a contiguous chunk
struct LightParameters
{
	float3		Position;
	float		Intensity;

	float3		Direction;
	float		Range;

	float3		Color;
	float		CosUmbra;			// Cosine of the half angle of umbra

	float3		Attenuation;
	float		CosPenumbra;		// Cosine of the half angle of penumbra
#if !USE_BOUNDING_SPHERES
	float3		_pad;
	uint		Type;
#endif
};

struct DirectionalLightParameters
{
#if SHADOWING_MAPPING
	float4x4 	LightMVP;
#endif

	float4		DirectionAndIntensity;
	
	float4		Color;
};

struct SSAODispatchParameters
{
	// Number of groups dispatched. (This parameter is not available as an MSL system value!)
    uint2   NumThreadGroups;

    // Total number of threads dispatched. (Also not available as an MSL system value!)
    // NOTE: This value may be less than the actual number of threads executed 
    //       if the screen size is not evenly divisible by the block size.
    uint2   NumThreads;
};

struct SSGIDispatchParameters
{
	// Number of groups dispatched. (This parameter is not available as an MSL system value!)
    uint2   NumThreadGroups;

    // Total number of threads dispatched. (Also not available as an MSL system value!)
    // NOTE: This value may be less than the actual number of threads executed 
    //       if the screen size is not evenly divisible by the block size.
    uint2   NumThreads;
};

#ifdef __cplusplus
static_assert((sizeof(PerObjectData) % 16) == 0, "Make sure PerObjectData is formatted in 16-byte chunks without any implicit padding.");
static_assert((sizeof(LightParameters) % 16) == 0, "Make sure LightParameters is formatted in 16-byte chunks without any implicit padding.");
// static_assert((sizeof(LightCullingLightInfo) % 16) == 0, "Make sure LightCullingLightInfo is formatted in 16-byte chunks without any implicit padding.");
static_assert((sizeof(DirectionalLightParameters) % 16) == 0, "Make sure DirectionalLightParameters is formatted in 16-byte chunks without any implicit padding.");
#endif