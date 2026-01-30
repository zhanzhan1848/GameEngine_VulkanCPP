#include <metal_stdlib>
#include "SH_Common.metal"

using namespace metal;

struct VertexInput
{
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float3 sh0      [[attribute(2)]];
    float3 sh1      [[attribute(3)]];
    float3 sh2      [[attribute(4)]];
};

struct VertexOutput
{
    float4 position [[position]];
    float3 color;
};

struct Uniforms
{
    float4x4 modelViewProjectionMatrix;
    float4x4 normalMatrix; 
    SH9Color envSH;        
};

vertex VertexOutput prt_baked_vertex(VertexInput in [[stage_in]],
                                     constant Uniforms& uniforms [[buffer(1)]])
{
    VertexOutput out;
    out.position = uniforms.modelViewProjectionMatrix * float4(in.position, 1.0);
    
    // Reconstruct Transfer SH from attributes
    SH9 transfer;
    transfer.c[0] = in.sh0.x;
    transfer.c[1] = in.sh0.y;
    transfer.c[2] = in.sh0.z;
    
    transfer.c[3] = in.sh1.x;
    transfer.c[4] = in.sh1.y;
    transfer.c[5] = in.sh1.z;
    
    transfer.c[6] = in.sh2.x;
    transfer.c[7] = in.sh2.y;
    transfer.c[8] = in.sh2.z;
    
    // Compute Irradiance
    out.color = DotSH9Color(transfer, uniforms.envSH);
    
    return out;
}

fragment float4 prt_baked_fragment(VertexOutput in [[stage_in]])
{
    return float4(in.color, 1.0);
}
