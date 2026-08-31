#include <metal_stdlib>
using namespace metal;

// Companion vertex shader for material-graph generated fragments
// (ShaderIR MetalEmitter world mode). The emitter hardcodes a six-member
// VertexOut; Metal pairs [[stage_in]] structs strictly by field order, so the
// declaration below must stay field-for-field identical to MetalEmitter.h's.
// Geometry/uniform plumbing mirrors GBuffer.metal's vertexMain: same AoS
// vertex layout (buffer(20)), instance array (buffer(3)) and push constants
// (buffer(2)).
#define RHI_ENABLE_PBR
#include "RHIShaderCommon.metal"

constant float InvIntervalsMG = 2.f / ((1 << 16) - 1);

struct MGVertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float2 uv;
    float4 shadowPos0;
    float4 shadowPos1;
};

struct MGSceneData {
    float4x4 model;
    float4 lightPos;
    float4 lightColor;
    float4 reflectionPlane;
    float4 reflectionPlane2;
    float4 reflectionPlane3;
    float4x4 previousModel;
    float2 jitter;
    float2 previousJitter;
    float  time;
    float  _timePad;
    float4 viewPos;
    float4x4 shadowMatrix0;
    float4x4 shadowMatrix1;
};

struct MGPushConsts {
    float4x4 model;
    uint use_instances;
    uint _pad[3];
};

struct MGViewData {
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float4x4 previousViewProjection;
};

#include "InstanceData.metal"

struct MGVertexElement {
    uint            ColorTSign;
    packed_ushort2  Normal;
    packed_ushort2  Tangent;
    packed_float2   UV;
};

struct MGVertexInput {
    packed_float3 position;
    MGVertexElement element;
};

float3 mgUnpackNormal(packed_ushort2 p, float normalZSign) {
    float2 f = float2(p);
    f = f * InvIntervalsMG - 1.0f;
    float d = dot(f, f);
    if (d > 1.0f) {
        // Invalid/Zeroed data fallback (matches GBuffer.metal)
        return float3(0.0f, 0.0f, normalZSign);
    }
    float z = sqrt(max(0.0f, 1.0f - d));
    return float3(f.x, f.y, z * normalZSign); // Z sign from ColorTSign bit 25
}

vertex MGVertexOut vertexMainMaterialGraph(
    uint vertexId [[vertex_id]],
    uint instanceId [[instance_id]],
    constant MGViewData& viewData [[buffer(0)]],
    constant MGSceneData& sceneData [[buffer(1)]],
    constant MGPushConsts& pushConsts [[buffer(2)]],
    constant InstanceData* instanceData [[buffer(3)]],
    constant MGVertexInput* vertices [[buffer(20)]]
) {
    MGVertexOut out;

    float4x4 model = (pushConsts.use_instances != 0)
        ? instanceData[instanceId].transform
        : pushConsts.model;

    float3 rawPos = vertices[vertexId].position;
    float normalZSign = (((vertices[vertexId].element).ColorTSign >> 25u) & 1u) != 0u ? 1.0f : -1.0f;
    float3 rawNormal = mgUnpackNormal(vertices[vertexId].element.Normal, normalZSign);
    float2 rawUV = vertices[vertexId].element.UV;

    float4 worldPos = model * float4(rawPos, 1.0);
    out.worldPos = worldPos.xyz;

    float3x3 normalMatrix = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    out.worldNormal = normalize(normalMatrix * rawNormal);

    // Same flip as GBuffer.metal so graph sampling matches fixed-path UVs.
    out.uv = float2(rawUV.x, 1.0 - rawUV.y);

    out.position = viewData.viewProjection * worldPos;

    out.shadowPos0 = sceneData.shadowMatrix0 * worldPos;
    out.shadowPos1 = sceneData.shadowMatrix1 * worldPos;

    return out;
}
