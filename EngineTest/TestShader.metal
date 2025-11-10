#include "Common.h"

#define ElementsTypeStaticNormal                0x01
#define ElementsTypeStaticNormalTexture         0x03
#define ElementsTypeStaticColor                 0x04
#define ElementsTypeSkeletal                    0x08
#define ElementsTypeSkeletalColor               0x0C  // 0x08 | 0x04
#define ElementsTypeSkeletalNormal              0x09  // 0x08 | 0x01
#define ElementsTypeSkeletalNormalColor         0x0D  // 0x08 | 0x01 | 0x04
#define ElementsTypeSkeletalNormalTexture       0x0B  // 0x08 | 0x03
#define ElementsTypeSkeletalNormalTextureColor  0x0F  // 0x08 | 0x03 | 0x04

struct VertexPosition
{
    device float3* positions;
};

struct VertexElement
{
    uint            ColorTSign;
    packed_ushort2         Normal;
    packed_ushort2         Tangent;
    packed_float2          UV;
};

struct VertexOut
{
    float4 HomogeneousPosition [[position]];
    float4 PreviousPosition;
	float3 WorldPosition;	
	float3 WorldNormal;		
	float3 WorldTangent;			
	float2 UV;		
};

struct PixelOut
{
    float4 World_Position [[color(0)]];
    float4 Normal_Depth [[color(1)]];
    float4 Albedo [[color(2)]];
    float4 MotionVector [[color(3)]];
};

constant float InvIntervals = 2.f / ((1 << 16) - 1);

struct GlobalData
{
    device const GlobalShaderData* global_shader_data [[id(0)]];
    device const PerObjectData* per_object_data [[id(1)]];
    device const packed_float3* vertices [[id(2)]];
    device const VertexElement* elements [[id(3)]];
    device const uint* srv_indices [[id(4)]];
};

struct StaticSamplerState
{
    sampler pointSampler [[id(0)]];
    sampler linearSampler [[id(1)]];
    sampler anisoSampler [[id(2)]];
};

Surface GetSurface(VertexOut psIn)
{
	Surface surface;
	surface.BaseColor = float3(1.f);
	surface.Metallic = 0.f;
	surface.Normal = normalize(psIn.WorldNormal);  // 修复：使用正确的法向量而不是标量值
	surface.PerceptualRoughness = 0.f;
	surface.EmissiveColor = float3(0.f);
	surface.EmissiveIntensity = 1.f;
	surface.AmbientOcclusion = 1.f;

#if TEXTURED_MTL
	surface.AmbientOcclusion = Sampler(0, LinearSampler, uv).r;
	surface.BaseColor = Sampler(1, LinearSampler, uv).rgb;
	surface.EmissiveColor = Sampler(2, LinearSampler, uv).rgb;
	float2 metalRough = Sampler(3, LinearSampler, uv).rg;  // 修复：语法错误，应该是.rg
	surface.Metallic = metalRough.r;
	surface.PerceptualRoughness = metalRough.g;
	surface.EmissiveIntensity = 1.f;
	float3 n = Sampler(4, LinearSampler, uv).rgb;
	surface.Normal = psIn.WorldNormal;
#endif

	return surface;
}

float LinearizeDepth(float depth)
{
    float nearClip = 0.1f;
    float farClip = 64.f;
    return (2.0 * nearClip * farClip) / (farClip + nearClip - depth * (farClip - nearClip));
}

VertexOut vertex vertex_main(device const GlobalData* global_data [[buffer(0)]],
                             uint vertex_index [[vertex_id]])
{
    VertexOut vsOut;

    // if ELEMENT_TYPE == 3
    float4 pos = float4(global_data->vertices[vertex_index], 1.f);
    float4 worldPosition = global_data->per_object_data->World * pos;

    VertexElement element = global_data->elements[vertex_index];
    float2 nXY = float2(element.Normal) * InvIntervals - 1.f;
    uint signs = (element.ColorTSign >> 24) & 0xff;
    float nSign = float(signs & 0x02) - 1;
    float3 normal = normalize(float3(nXY.x, nXY.y, sqrt(clamp(1.f - dot(nXY, nXY), 0.f, 1.f)) * nSign));

    float2 tXY = float2(element.Tangent) * InvIntervals - 1.f;
    float tSign = float(signs & 0x01) - 1; // 假设切线符号在位 0
    float3 tangent = float3(tXY.x, tXY.y, sqrt(clamp(1.f - dot(tXY, tXY), 0.f, 1.f)) * tSign);

    vsOut.HomogeneousPosition = global_data->per_object_data->WorldViewProjection * worldPosition;
    vsOut.PreviousPosition = global_data->global_shader_data->PreviousViewProjection * worldPosition;
    vsOut.WorldPosition	  = worldPosition.xyz;
    vsOut.WorldNormal	  = (transpose(global_data->per_object_data->InvWorld) * float4(normal, 0.f)).xyz;
    vsOut.WorldTangent	  = (global_data->per_object_data->World * float4(tangent, 0.f)).xyz;;
    vsOut.UV			  = element.UV;

    return vsOut;
}

PixelOut fragment fragment_main(VertexOut vsOut [[stage_in]],
                             device const GlobalData* global_data [[buffer(0)]],
                             device StaticSamplerState& static_sampler_state [[buffer(1)]]
                            )
{
    PixelOut psOut;
    float3 color = float3(0.f);
    float3 viewDir = normalize(global_data->global_shader_data->CameraPositionAndViewWidth.xyz - vsOut.WorldPosition);

    Surface S = GetSurface(vsOut);

    // 输出颜色
    psOut.World_Position = float4(vsOut.WorldPosition, 1.f);
    psOut.Albedo = float4(S.BaseColor + S.EmissiveColor * S.EmissiveIntensity, 1.f);
    // 输出法线和深度
    float depth = vsOut.HomogeneousPosition.z;
    // 使用正确的线性深度计算函数
    float linearDepth = LinearizeDepth(depth);
    psOut.Normal_Depth = float4(normalize(vsOut.WorldNormal), linearDepth);

    // Motion vector
    float2 currentUV = (vsOut.HomogeneousPosition.xy / vsOut.HomogeneousPosition.w) * 0.5f + 0.5f;
    float2 previousUV = (vsOut.PreviousPosition.xy / vsOut.PreviousPosition.w) * 0.5f + 0.5f;
    psOut.MotionVector = float4(previousUV - currentUV, 1.f, 1.f);
    
    return psOut;
}