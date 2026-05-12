#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "SurfaceCacheData.metal"

struct CaptureVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
};

struct CaptureVertexOut {
    float4 position [[position]];
    float2 uv;
    float3 world_normal;
    float3 world_pos;
    uint   material_id;
};

struct CapturePassData {
    SurfaceCacheCard card;
    uint instance_offset;
    uint _pad[3];
};

// Build ortho view matrix from card axis/direction
vertex CaptureVertexOut surfaceCacheCaptureVS(
    CaptureVertexIn in [[stage_in]],
    constant CapturePassData& pass [[buffer(1)]],
    constant float4x4* instances [[buffer(2)]])
{
    CaptureVertexOut out;
    float4x4 world = instances[pass.instance_offset];
    float4 wp = world * float4(in.position, 1.0);
    float3 wn = normalize((world * float4(in.normal, 0.0)).xyz);

    uint axis = pass.card.axis_direction & 0xFF;
    uint dir = (pass.card.axis_direction >> 8) & 0xFF;

    float3 fwd, right, up;
    if (axis == 0) { right=float3(0,1,0); up=float3(0,0,1); fwd=float3(dir?1:-1,0,0); }
    else if (axis == 1) { right=float3(1,0,0); up=float3(0,0,1); fwd=float3(0,dir?1:-1,0); }
    else { right=float3(1,0,0); up=float3(0,1,0); fwd=float3(0,0,dir?1:-1); }

    float3 origin = pass.card.center.xyz;
    float4x4 view;
    view[0]=float4(right.x,up.x,fwd.x,0);
    view[1]=float4(right.y,up.y,fwd.y,0);
    view[2]=float4(right.z,up.z,fwd.z,0);
    view[3]=float4(-dot(right,origin),-dot(up,origin),-dot(fwd,origin),1);

    float rx = (axis==0) ? pass.card.extent.x : pass.card.extent.y;
    float ry = (axis==0) ? pass.card.extent.z : ((axis==1) ? pass.card.extent.z : pass.card.extent.y);
    float rz = pass.card.extent.x;
    float4x4 proj;
    proj[0]=float4(1.0/max(rx,0.001),0,0,0);
    proj[1]=float4(0,1.0/max(ry,0.001),0,0);
    proj[2]=float4(0,0,1.0/max(rz,0.001),0);
    proj[3]=float4(0,0,0,1);

    out.position = proj * view * wp;
    out.world_pos = wp.xyz;
    out.world_normal = wn;
    out.uv = in.uv;
    out.material_id = 0;
    return out;
}

// MUST be separate functions — Apple Silicon inline texture sampling bug
static float4 sampleAlbedo(uint mid, float2 uv, texture2d_array<float> tex) {
    constexpr sampler s(coord::normalized, filter::linear, address::repeat);
    return tex.sample(s, uv, mid);
}

struct CaptureOutput {
    float4 albedo  [[color(0)]];
    float4 normal  [[color(1)]];
    float  depth   [[color(2)]];
    float4 emissive [[color(3)]];
};

fragment CaptureOutput surfaceCacheCaptureFS(
    CaptureVertexOut in [[stage_in]],
    texture2d_array<float> albedo_array [[texture(0)]],
    texture2d_array<float> normal_array [[texture(1)]],
    texture2d_array<float> orm_array    [[texture(2)]],
    constant CapturePassData& pass      [[buffer(1)]])
{
    CaptureOutput out;
    out.albedo = sampleAlbedo(in.material_id, in.uv, albedo_array);

    float3 wn = normalize(in.world_normal);
    out.normal = float4(octEncode(wn), 0.0, 1.0);

    float3 local = in.world_pos - pass.card.center.xyz;
    uint axis = pass.card.axis_direction & 0xFF;
    float depth;
    if (axis == 0) depth = abs(local.x);
    else if (axis == 1) depth = abs(local.y);
    else depth = abs(local.z);
    out.depth = depth;

    // TODO: sample emissive from material data when available
    out.emissive = float4(0.0, 0.0, 0.0, 1.0);
    return out;
}
