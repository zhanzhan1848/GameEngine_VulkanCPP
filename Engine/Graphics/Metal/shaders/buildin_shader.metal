#include "Common.h"

// Vertex Pulling Structures (Must match C++ side layout in MetalGPass.cpp)
struct VertexElement
{
    uint            ColorTSign;
    packed_ushort2  Normal;
    packed_ushort2  Tangent;
    packed_float2   UV;
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

float LinearizeDepth(float depth)
{
    // TODO: Pass near/far planes via GlobalShaderData
    float nearClip = 0.1f;
    float farClip = 64.f;
    return (2.0 * nearClip * farClip) / (farClip + nearClip - depth * (farClip - nearClip));
}

vertex VertexOut vertex_main(device const GlobalData* global_data [[buffer(0)]],
                             uint vertex_index [[vertex_id]])
{
    VertexOut vsOut;

    // Fetch vertex position
    float4 pos = float4(global_data->vertices[vertex_index], 1.f);
    float4 worldPosition = global_data->per_object_data->World * pos;

    // Fetch and decode vertex attributes
    VertexElement element = global_data->elements[vertex_index];
    
    // Decode Normal
    float2 nXY = float2(element.Normal) * InvIntervals - 1.f;
    uint signs = (element.ColorTSign >> 24) & 0xff;
    float nSign = float(signs & 0x02) - 1;
    float3 normal = normalize(float3(nXY.x, nXY.y, sqrt(clamp(1.f - dot(nXY, nXY), 0.f, 1.f)) * nSign));

    // Decode Tangent
    float2 tXY = float2(element.Tangent) * InvIntervals - 1.f;
    float tSign = float(signs & 0x01) - 1; 
    float3 tangent = float3(tXY.x, tXY.y, sqrt(clamp(1.f - dot(tXY, tXY), 0.f, 1.f)) * tSign);

    // Transform to world space and clip space
    vsOut.HomogeneousPosition = global_data->per_object_data->WorldViewProjection * worldPosition;
    vsOut.PreviousPosition = global_data->global_shader_data->PreviousViewProjection * worldPosition;
    vsOut.WorldPosition	  = worldPosition.xyz;
    vsOut.WorldNormal	  = (transpose(global_data->per_object_data->InvWorld) * float4(normal, 0.f)).xyz;
    vsOut.WorldTangent	  = (global_data->per_object_data->World * float4(tangent, 0.f)).xyz;
    vsOut.UV			  = element.UV;

    return vsOut;
}

fragment PixelOut fragment_main(VertexOut vsOut [[stage_in]],
                                device const GlobalData* global_data [[buffer(0)]],
                                device StaticSamplerState& static_sampler_state [[buffer(1)]])
{
    PixelOut psOut;

    // Basic Surface Properties (Placeholder for material system)
    Surface S;
    S.BaseColor = float3(0.8f); // Default grey
    S.Metallic = 0.0f;
    S.PerceptualRoughness = 0.5f;
    S.EmissiveColor = float3(0.0f);
    S.EmissiveIntensity = 0.0f;
    
    // Normal mapping could go here
    float3 N = normalize(vsOut.WorldNormal);

    // -------------------------------------------------------------------------
    // SH Light Probe Integration
    // -------------------------------------------------------------------------
    // Calculate Irradiance from SH coefficients stored in PerObjectData
    float3 irradiance = EvalSH9Irradiance(N, global_data->per_object_data->sh_coeffs);
    
    // Apply irradiance as ambient light
    // Note: In a full PBR pipeline, this would be part of the indirect diffuse calculation
    float3 ambient = S.BaseColor * irradiance;
    // -------------------------------------------------------------------------

    // Output to GBuffer
    psOut.World_Position = float4(vsOut.WorldPosition, 1.f);
    
    // Albedo + Ambient (SH)
    psOut.Albedo = float4(S.BaseColor + S.EmissiveColor * S.EmissiveIntensity + ambient, 1.f);
    
    // Normal + Linear Depth
    float depth = vsOut.HomogeneousPosition.z;
    float linearDepth = LinearizeDepth(depth);
    psOut.Normal_Depth = float4(N, linearDepth);

    // Motion Vectors
    float2 currentUV = (vsOut.HomogeneousPosition.xy / vsOut.HomogeneousPosition.w) * 0.5f + 0.5f;
    float2 previousUV = (vsOut.PreviousPosition.xy / vsOut.PreviousPosition.w) * 0.5f + 0.5f;
    psOut.MotionVector = float4(previousUV - currentUV, 1.f, 1.f);
    
    return psOut;
}
