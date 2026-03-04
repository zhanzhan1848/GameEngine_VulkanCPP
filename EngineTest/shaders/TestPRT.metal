#include <metal_stdlib>
#include "SH_Common.metal"

using namespace metal;

struct VertexInput
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
};

struct VertexOutput
{
    float4 position [[position]];
    float3 color;
};

struct Uniforms
{
    float4x4 modelViewProjectionMatrix;
    float4x4 normalMatrix; // To transform normals to world space
    SH9Color envSH;        // Environment SH coefficients (9 * float3)
};

// Cosine convolution factors
constant float SH_COSINE_A0 = 3.14159265359f;
constant float SH_COSINE_A1 = 2.09439510239f;
constant float SH_COSINE_A2 = 0.78539816339f;

vertex VertexOutput prt_vertex(VertexInput in [[stage_in]],
                               constant Uniforms& uniforms [[buffer(1)]])
{
    VertexOutput out;
    out.position = uniforms.modelViewProjectionMatrix * float4(in.position, 1.0);
    
    // Transform normal to world space (assuming rigid transform for now)
    float3 worldNormal = normalize((uniforms.normalMatrix * float4(in.normal, 0.0)).xyz);
    
    // 1. Evaluate SH Basis for normal
    SH9 basis = EvalSHBasis9(worldNormal);
    
    // 2. Compute Transfer SH (Cosine Lobe oriented at Normal)
    // T_lm = A_l * Y_lm(n)
    SH9 transfer;
    transfer.c[0] = SH_COSINE_A0 * basis.c[0];
    
    transfer.c[1] = SH_COSINE_A1 * basis.c[1];
    transfer.c[2] = SH_COSINE_A1 * basis.c[2];
    transfer.c[3] = SH_COSINE_A1 * basis.c[3];
    
    transfer.c[4] = SH_COSINE_A2 * basis.c[4];
    transfer.c[5] = SH_COSINE_A2 * basis.c[5];
    transfer.c[6] = SH_COSINE_A2 * basis.c[6];
    transfer.c[7] = SH_COSINE_A2 * basis.c[7];
    transfer.c[8] = SH_COSINE_A2 * basis.c[8];
    
    // 3. Compute Irradiance
    out.color = DotSH9Color(transfer, uniforms.envSH);
    
    // Tone mapping or gamma correction could be done here or in frag
    // For test, just output linear
    
    return out;
}

fragment float4 prt_fragment(VertexOutput in [[stage_in]])
{
    return float4(in.color, 1.0);
}
