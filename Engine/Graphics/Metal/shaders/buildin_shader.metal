#include "Common.h"

struct VertexOutput
{
    float4 position [[position]];
    float2 uv;
};

vertex VertexOutput vertex_main(
    uint vertexID [[vertex_id]])
{
    VertexOutput o;
    o.uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.position = float4(o.uv * 2.0f - 1.0f, 0.0f, 1.0f);
	return o;
}

fragment float4 fragment_main(VertexOutput fragInput [[stage_in]])
{
    return float4(fragInput.uv * 0.5f, 0.0f, 1.0f);
}