#include "Common.h"
#include "CommonTypes.metal"

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
	float3 WorldPosition;	
	float3 WorldNormal;		
	float3 WorldTangent;			
	float2 UV;		
};

constant float InvIntervals = 2.f / ((1 << 16) - 1);

struct GlobalData
{
    device const GlobalShaderData* global_shader_data [[id(0)]];
    device const PerObjectData* per_object_data [[id(1)]];
    device const packed_float3* vertices [[id(2)]];
    device const VertexElement* elements [[id(3)]];
};

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

    vsOut.HomogeneousPosition = global_data->per_object_data->WorldViewProjection * pos;
    vsOut.WorldPosition	  = worldPosition.xyz;
    vsOut.WorldNormal	  = (global_data->per_object_data->InvWorld * float4(normal, 0.f)).xyz;
    vsOut.WorldTangent	  = (global_data->per_object_data->World * float4(tangent, 0.f)).xyz;;
    vsOut.UV			  = element.UV;

    return vsOut;
}

float4 fragment fragment_main(VertexOut vsOut [[stage_in]])
{
    float4 color;
    float3 normalizedNormal = normalize(vsOut.WorldNormal);
    color = float4(normalizedNormal * 0.5f + 0.5f, 1.f);

    return color;
}