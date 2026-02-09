#include <metal_stdlib>
using namespace metal;

#define RHI_ENABLE_PBR
#include "RHIShaderCommon.metal"

constant float InvIntervals = 2.f / ((1 << 16) - 1);

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float3 worldTangent;
    float3 worldBitangent;
    float2 uv;
    float4 currentPos;
    float4 previousPos;
};

struct FragmentOut {
    float4 albedo [[color(0)]];
    float4 normal [[color(1)]];
    float4 orm [[color(2)]];
    float2 velocity [[color(3)]];
};

struct SceneData {
    float4x4 model;
    float4 lightPos;
    float4 lightColor;
    float4 reflectionPlane;
    float4 reflectionPlane2;
    float4 reflectionPlane3;
    float4x4 previousModel;
    float2 jitter;
    float2 previousJitter;
    float2 padding;
    float4 viewPos;
    float4x4 shadowMatrix0;
    float4x4 shadowMatrix1;
};

struct PushConsts {
    float4x4 model;
};

struct ViewData {
    float4x4 viewProjection;
    float4x4 invViewProjection; // Added to match C++
    float4x4 previousViewProjection;
};

struct VertexElement {
    uint            ColorTSign; 
    packed_ushort2  Normal; 
    packed_ushort2  Tangent; 
    packed_float2   UV;
};

struct VertexInput {
    packed_float3 position;
    VertexElement element;
};

// Helper Functions
float3 UnpackNormal(packed_ushort2 p) {
    float2 f = float2(p);
    f = f * InvIntervals - 1.0f;
    float z = sqrt(max(0.0f, 1.0f - dot(f, f)));
    return float3(f.x, f.y, z); // Assuming Z is reconstructed positive
}

vertex VertexOut vertexMain(
    uint vertexId [[vertex_id]],
    constant SceneData& sceneData [[buffer(0)]],
    constant ViewData& viewData [[buffer(1)]],
    constant PushConsts& pushConsts [[buffer(2)]],
    device const VertexInput* vertices [[buffer(20)]]
) {
    VertexOut out;
    
    // Fetch Attributes
    float3 rawPos = vertices[vertexId].position;
    VertexElement element = vertices[vertexId].element;
    
    // Unpack Normal
    packed_ushort2 packedN = element.Normal;
    float3 rawNormal = UnpackNormal(packedN);
    
    // Unpack Tangent
    packed_ushort2 packedT = element.Tangent;
    float3 rawTangent = UnpackNormal(packedT); // Tangent uses same encoding
    
    // Unpack UV
    float2 rawUV = element.UV;
    
    // Sign from ColorTSign
    // uint colorTSign = element.ColorTSign;
    // float sign = (colorTSign & 0x1) ? -1.0 : 1.0; // Assume sign bit
    
    // Use PushConstant Model Matrix
    float4 worldPos = pushConsts.model * float4(rawPos, 1.0);
    
    out.worldPos = worldPos.xyz;
    
    // Transform Normal to World Space
    float3x3 normalMatrix = float3x3(pushConsts.model[0].xyz, pushConsts.model[1].xyz, pushConsts.model[2].xyz);
    out.worldNormal = normalize(normalMatrix * rawNormal);
    
    // Transform Tangent to World Space
    out.worldTangent = normalize(normalMatrix * rawTangent);
    
    // Calculate Bitangent
    // Using reconstructed tangent frame
    // out.worldBitangent = cross(out.worldNormal, out.worldTangent) * sign;
    // For now, simple cross
    out.worldBitangent = cross(out.worldNormal, out.worldTangent);
    
    out.uv = rawUV;
    
    out.position = viewData.viewProjection * worldPos;
    
    // TAA Jitter
    float4 currentPos = out.position;
    // Apply Jitter only if TAA is enabled
    // out.position.xy += sceneData.jitter * out.position.w; 
    
    out.currentPos = currentPos;
    out.previousPos = viewData.previousViewProjection * (sceneData.previousModel * float4(rawPos, 1.0));
    
    return out;
}

fragment FragmentOut fragmentMain(
    VertexOut in [[stage_in]],
    constant SceneData& sceneData [[buffer(0)]],
    texture2d<float> albedoMap [[texture(0)]],
    texture2d<float> normalMap [[texture(1)]],
    texture2d<float> ormMap [[texture(2)]],
    sampler defaultSampler [[sampler(3)]]
) {
    FragmentOut out;
    
    // Debug: Visualize Vertex Data State
    /*
    if (in.uv.x > 0.5) {
        // Valid Data: Yellow
        out.albedo = float4(1.0, 1.0, 0.0, 1.0);
        // Also show Normal to verify unpacking
        // out.albedo = float4(in.worldNormal * 0.5 + 0.5, 1.0);
    } else {
        // Invalid Data (Zero): Red
        out.albedo = float4(1.0, 0.0, 0.0, 1.0);
    }
    */
    
    // DEBUG: Visualize UV Checkerboard to verify geometry and UV scale
    // float2 checkUV = in.uv * 10.0;
    // float check = fmod(floor(checkUV.x) + floor(checkUV.y), 2.0);
    // out.albedo = float4(check, check, check, 1.0);

    // out.albedo = float4(in.worldNormal * 0.5 + 0.5, 1.0);

    // DEBUG: Output UV as color
    // out.albedo = float4(in.uv, 0.0, 1.0);
    
    // DEBUG: Force Red to verify GBuffer execution and data flow
    // out.albedo = float4(1.0, 0.0, 0.0, 1.0);

    // Standard PBR Sampling
    out.albedo = albedoMap.sample(defaultSampler, in.uv);
    // DEBUG: Force Red to verify GBuffer execution
    // out.albedo = float4(1.0, 0.0, 0.0, 1.0);
    // out.albedo = float4(1.0, 1.0, 1.0, 1.0);
    
    // Normal Mapping
    float3 normal = normalMap.sample(defaultSampler, in.uv).rgb;
    normal = normal * 2.0 - 1.0;
    
    // float3 T = normalize(in.worldTangent);
    float3 N = normalize(in.worldNormal);
    float3 T = normalize(in.worldTangent);
    // float3 B = normalize(in.worldBitangent); // Use Bitangent from vertex
    // If Bitangent is not available, calculate it:
    float3 B = cross(N, T); 
    float3x3 TBN = float3x3(T, B, N);
    
    out.normal = float4(normalize(TBN * normal) * 0.5 + 0.5, 1.0);
    // out.normal = float4(N * 0.5 + 0.5, 1.0);

    out.orm = ormMap.sample(defaultSampler, in.uv);
    // out.orm = float4(1.0, 0.5, 0.0, 1.0); // Occlusion=1.0, Roughness=0.5, Metallic=0.0
    
    // Calculate Velocity
    // Convert to NDC
    float2 currentNDC = in.currentPos.xy / in.currentPos.w;
    float2 previousNDC = in.previousPos.xy / in.previousPos.w;
    
    // Remove Jitter to get pure geometric motion
    // We want the velocity to be 0 for static objects, so we must remove the jitter offset
    float2 currentNDC_NoJitter = currentNDC - sceneData.jitter;
    float2 previousNDC_NoJitter = previousNDC - sceneData.previousJitter;
    
    out.velocity = (currentNDC_NoJitter - previousNDC_NoJitter) * 0.5;
    
    return out;
}
