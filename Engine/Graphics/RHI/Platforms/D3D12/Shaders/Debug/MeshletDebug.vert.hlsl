// Vulkan reference: Debug/MeshletDebug.vert (main).
struct MeshletHeader
{
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
    float4 cone0;
    float4 cone1;
    float4 cone2;
};

ByteAddressBuffer triIndices : register(t3, space0);
ByteAddressBuffer meshletBuffer : register(t1, space0);
ByteAddressBuffer vertexIndices : register(t2, space0);
ByteAddressBuffer positions : register(t4, space0);
cbuffer DebugUniforms : register(b0, space0)
{
    row_major float4x4 viewProjection : packoffset(c0);
    row_major float4x4 model : packoffset(c4);
    uint mesh_id : packoffset(c8);
    float wireframe_enabled : packoffset(c8.y);
    float4 pad0 : packoffset(c9);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float3 vColor : TEXCOORD0;
    float vAlpha : TEXCOORD1;
    float3 vBarycentric : TEXCOORD2;
    float4 positionCS : SV_Position;
};

uint readTriangleByte(uint byteIndex)
{
    uint _packed = triIndices.Load((byteIndex >> 2u) * 4);
    return (_packed >> (8u * (byteIndex & 3u))) & 255u;
}

float3 hashColor(uint n) {
    float r = frac(sin(float(n) * 12.9898) * 43758.5453);
    float g = frac(sin(float(n + 1) * 78.233) * 43758.5453);
    float b = frac(sin(float(n + 2) * 45.164) * 43758.5453);
    return float3(0.3 + 0.7 * r, 0.3 + 0.7 * g, 0.3 + 0.7 * b);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint vertexID = uint(int(input.vertexID));
    uint instanceID = uint(int(input.instanceID));
    MeshletHeader _106;
    _106.vertex_offset = meshletBuffer.Load(instanceID * 64);
    _106.triangle_offset = meshletBuffer.Load(instanceID * 64 + 4);
    _106.vertex_count = meshletBuffer.Load(instanceID * 64 + 8);
    _106.triangle_count = meshletBuffer.Load(instanceID * 64 + 12);
    _106.cone0 = asfloat(meshletBuffer.Load4(instanceID * 64 + 16));
    _106.cone1 = asfloat(meshletBuffer.Load4(instanceID * 64 + 32));
    _106.cone2 = asfloat(meshletBuffer.Load4(instanceID * 64 + 48));
    MeshletHeader meshlet;
    meshlet.vertex_offset = _106.vertex_offset;
    meshlet.triangle_offset = _106.triangle_offset;
    meshlet.vertex_count = _106.vertex_count;
    meshlet.triangle_count = _106.triangle_count;
    meshlet.cone0 = _106.cone0;
    meshlet.cone1 = _106.cone1;
    meshlet.cone2 = _106.cone2;
    uint triIndex = vertexID;
    if (triIndex >= (meshlet.triangle_count * 3u))
    {
        output.positionCS = 0.0f.xxxx;
        output.vColor = 0.0f.xxx;
        output.vAlpha = 0.0f;
        output.vBarycentric = 0.0f.xxx;
        return output;
    }
    uint localVertIdx = readTriangleByte(meshlet.triangle_offset + triIndex);
    uint vertIdx = vertexIndices.Load((meshlet.vertex_offset + localVertIdx) * 4);
    float3 pos = float3(asfloat(positions.Load(((3u * vertIdx) + 0u) * 4)), asfloat(positions.Load(((3u * vertIdx) + 1u) * 4)), asfloat(positions.Load(((3u * vertIdx) + 2u) * 4)));
    float4 worldPos = mul(float4(pos, 1.0f), model);
    output.positionCS = mul(worldPos, viewProjection);
    uint seed = instanceID + (mesh_id * 1024u);
    output.vColor = hashColor(seed);
    output.vAlpha = 0.6f;
    uint vertexInTriangle = vertexID % 3u;
    if (vertexInTriangle == 0u)
    {
        output.vBarycentric = float3(1.0f, 0.0f, 0.0f);
    }
    else
    {
        if (vertexInTriangle == 1u)
        {
            output.vBarycentric = float3(0.0f, 1.0f, 0.0f);
        }
        else
        {
            output.vBarycentric = float3(0.0f, 0.0f, 1.0f);
        }
    }
    return output;
}
